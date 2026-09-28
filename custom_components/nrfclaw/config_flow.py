"""Config flow for nRFClaw automatic HA transport-role discovery."""

from __future__ import annotations

from homeassistant import config_entries
from homeassistant.components import bluetooth
from homeassistant.components.bluetooth import BluetoothServiceInfoBleak
import voluptuous as vol

from .const import (
    CONF_ACCESS_KEY,
    CONF_MODE,
    CONF_POLL_INTERVAL,
    DEFAULT_DIRECT_POLL_INTERVAL,
    DOMAIN,
    MODE_BRIDGE,
    MODE_DIRECT,
)
from .direct_adv import (
    MANUFACTURER_ID,
    HA_ROLE_BRIDGE,
    HA_ROLE_DIRECT_BLE,
    advertised_ha_role,
)

# A discovery flow can be opened by any matching manifest entry. In B7.6f the
# service-UUID matcher can therefore arrive before the manufacturer frame that
# carries the v2 HA role. Wait briefly for the complete advertisement instead
# of incorrectly falling back to the legacy/manual role selector.
ADDITIONAL_ROLE_DISCOVERY_TIMEOUT = 8.0


def _advertised_role(service_info: BluetoothServiceInfoBleak | None) -> int | None:
    if service_info is None:
        return None
    payload = (service_info.manufacturer_data or {}).get(MANUFACTURER_ID)
    return advertised_ha_role(bytes(payload)) if payload is not None else None


class NrfClawConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def _async_resolve_advertised_role(
        self,
        discovery_info: BluetoothServiceInfoBleak,
    ) -> int | None:
        """Resolve the explicit B7.6f role from a complete advertisement."""

        role = _advertised_role(discovery_info)
        if role is not None:
            return role

        # The Bluetooth manager may already have a newer merged report than
        # the one that opened this config flow.
        latest = bluetooth.async_last_service_info(
            self.hass,
            discovery_info.address,
            connectable=False,
        )
        role = _advertised_role(latest)
        if role is not None:
            return role

        # Official HA Bluetooth pattern for devices whose first discovery
        # packet can be incomplete: wait for the next useful advertisement.
        def _has_explicit_role(service_info: BluetoothServiceInfoBleak) -> bool:
            return _advertised_role(service_info) is not None

        try:
            complete = await bluetooth.async_process_advertisements(
                self.hass,
                _has_explicit_role,
                {"address": discovery_info.address},
                bluetooth.BluetoothScanningMode.ACTIVE,
                ADDITIONAL_ROLE_DISCOVERY_TIMEOUT,
            )
        except TimeoutError:
            return None

        return _advertised_role(complete)

    async def async_step_bluetooth(self, discovery_info: BluetoothServiceInfoBleak):
        await self.async_set_unique_id(discovery_info.address)
        self._abort_if_unique_id_configured()
        self.context["title_placeholders"] = {
            "name": discovery_info.name or "nRFClaw"
        }

        role = await self._async_resolve_advertised_role(discovery_info)
        if role == HA_ROLE_DIRECT_BLE:
            self.context["nrfclaw_auto_mode"] = MODE_DIRECT
        elif role == HA_ROLE_BRIDGE:
            self.context["nrfclaw_auto_mode"] = MODE_BRIDGE
        else:
            # B7.6f2h1: supported NC v1/v2 devices are role-bearing.
            # Fail closed and wait for Bluetooth rediscovery instead of
            # asking the user to guess Direct vs Bridge.
            return self.async_abort(reason="role_not_resolved")

        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input=None):
        auto_mode = self.context.get("nrfclaw_auto_mode")

        if user_input is not None:
            access_key = user_input.get(CONF_ACCESS_KEY, "").strip()
            mode = auto_mode or user_input.get(CONF_MODE, MODE_DIRECT)

            data = {
                "address": self.unique_id,
                CONF_ACCESS_KEY: access_key,
                CONF_MODE: mode,
            }
            if mode == MODE_DIRECT:
                # B7.6d+ Direct telemetry is advertisement-driven. Keep the
                # historical field in entry data for compatibility, but do not
                # ask the user to configure a poll interval for v2 auto mode.
                data[CONF_POLL_INTERVAL] = int(
                    user_input.get(
                        CONF_POLL_INTERVAL,
                        DEFAULT_DIRECT_POLL_INTERVAL,
                    )
                )

            title = self.context.get("title_placeholders", {}).get(
                "name", "nRFClaw"
            )
            title = f"{title} {'Bridge' if mode == MODE_BRIDGE else 'Direct'}"
            return self.async_create_entry(title=title, data=data)

        fields: dict = {
            vol.Optional(CONF_ACCESS_KEY, default=""): str,
        }

        if auto_mode is None:
            # Legacy fallback only. B7.6f v2 Direct/Bridge devices must be
            # classified from manufacturer data, not by user selection.
            fields[vol.Required(CONF_MODE, default=MODE_DIRECT)] = vol.In(
                {
                    MODE_DIRECT: "Direct NDP sensor (legacy/manual)",
                    MODE_BRIDGE: "NinaLink bridge (legacy/manual)",
                }
            )
            fields[
                vol.Optional(
                    CONF_POLL_INTERVAL,
                    default=DEFAULT_DIRECT_POLL_INTERVAL,
                )
            ] = vol.All(
                vol.Coerce(int),
                vol.Range(min=5, max=3600),
            )

        return self.async_show_form(
            step_id="confirm",
            data_schema=vol.Schema(fields),
            description_placeholders={
                "detected_mode": auto_mode or "legacy",
            },
        )

    async def async_step_user(self, user_input=None):
        return self.async_abort(reason="bluetooth_discovery_required")
