from __future__ import annotations

from homeassistant import config_entries
import voluptuous as vol
from homeassistant.components.bluetooth import BluetoothServiceInfoBleak

from .const import DOMAIN


class NrfClawConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_bluetooth(self, discovery_info: BluetoothServiceInfoBleak):
        await self.async_set_unique_id(discovery_info.address)
        self._abort_if_unique_id_configured()
        self.context["title_placeholders"] = {
            "name": discovery_info.name or "nRFClaw"
        }
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input=None):
        if user_input is not None:
            access_key = user_input.get("access_key", "").strip()
            return self.async_create_entry(
                title=self.context.get("title_placeholders", {}).get(
                    "name", "nRFClaw"
                ),
                data={"address": self.unique_id, "access_key": access_key},
            )
        return self.async_show_form(
            step_id="confirm",
            data_schema=vol.Schema({vol.Optional("access_key", default=""): str}),
        )

    async def async_step_user(self, user_input=None):
        return self.async_abort(reason="bluetooth_discovery_required")
