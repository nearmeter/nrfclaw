#!/usr/bin/env python3
from __future__ import annotations
from pathlib import Path
import json, re, unicodedata
from typing import Any

KB_PATH=Path(__file__).resolve().parent/'nrfclaw_knowledge.json'

def _fold(s:str)->str:
    s=unicodedata.normalize('NFKD',s)
    s=''.join(c for c in s if not unicodedata.combining(c)).lower()
    return re.sub(r'\s+',' ',s).strip()

def load_kb()->dict[str,Any]:
    return json.loads(KB_PATH.read_text(encoding='utf-8'))

def _years(capacity_mah:float,current_uA:float)->float:
    return capacity_mah/(current_uA/1000.0)/24.0/365.0

def answer_question(question:str)->dict[str,Any]:
    q=_fold(question); kb=load_kb()
    # B7.6f2o1a: NinaLink/NDP local product-QA parity.
    english = bool(re.search(
        r"\b(?:what|difference|explain|how|create|add|between|network|key|bridge|node)\b",
        q,
    ))

    asks_network_vs_key = (
        ("ndp" in q)
        and (("network id" in q) or ("network" in q) or ("id de rede" in q) or ("rede" in q))
        and (("key" in q) or ("chave" in q))
    )
    if asks_network_vs_key:
        net = kb["ninalink"]["network_id"]
        key = kb["home_assistant"]["ndp_owner_key"]
        if english:
            answer = (
                f"The NinaLink Network ID is a {net['bits']}-bit network-isolation/admission value. "
                f"{net['unprovisioned_value']} means legacy/unprovisioned and prompt provisioning uses "
                f"{net['prompt_range']}. It decides which LoRa nodes belong to the same NinaLink network; "
                "it is not an authentication secret. "
                f"The NDP access key is an optional {key['size_bits']}-bit owner/authentication key for "
                "Home Assistant/Application-NDP access to a Direct device or NinaLink Bridge. "
                f"Generate it through the physical P0.21/NUS owner plane with `{key['generate_command']}` "
                f"and enter the resulting `{key['format']}` value in Home Assistant's "
                f"`{key['home_assistant_field']}` field."
            )
        else:
            answer = (
                f"O NinaLink Network ID é um valor de {net['bits']} bits para isolamento/admissão da rede. "
                f"{net['unprovisioned_value']} significa legado/não provisionado e o provisionamento por prompt usa "
                f"{net['prompt_range']}. Ele define quais nodes LoRa pertencem à mesma rede NinaLink; "
                "não é um segredo de autenticação. "
                f"A NDP access key é uma chave opcional de {key['size_bits']} bits para autenticar o acesso "
                "Home Assistant/Application-NDP a um dispositivo Direct ou Bridge NinaLink. "
                f"Gere-a pelo plano físico P0.21/NUS com `{key['generate_command']}` e copie o valor "
                f"`{key['format']}` para o campo `{key['home_assistant_field']}` do Home Assistant."
            )
        return {"topic": "ninalink-network-vs-ndp-key", "answer": answer, "confidence": "high"}

    asks_bridge_node = (
        ("bridge" in q)
        and ("node" in q)
        and (
            ("how" in q) or ("explain" in q) or ("create" in q) or ("add" in q)
            or ("como" in q) or ("criar" in q) or ("adicionar" in q)
        )
    )
    if asks_bridge_node:
        nl = kb["ninalink"]
        period = int(nl["default_node_report_period_s"])
        if english:
            answer = (
                "1. Put the future bridge in the physical P0.21/NUS programming plane and upload "
                f"`{nl['bridge_prompt']}` with `prompt --standalone --upload --boot`. "
                "The CLI reuses an existing non-zero Network ID or creates and persists one. "
                "2. Copy the Network ID printed by the bridge (or read it with `ninalink-network-status`). "
                "3. Put the battery node in P0.21/NUS and upload "
                f"`{nl['node_prompt']}` using the bridge's actual ID instead of 0x1234. "
                f"The node runs as a low-power NinaLink Node: Application/NDP is off and the default "
                f"LoRa contact/report period is {period} s, with the LLCC68 and MCU returning to the "
                "low-power path between contacts. "
                "The Bridge keeps continuous LoRa RX and exposes the remote node to Home Assistant over BLE/NDP."
            )
        else:
            answer = (
                "1. Coloque a futura bridge no plano físico P0.21/NUS e grave "
                f"`{nl['bridge_prompt']}` com `prompt --standalone --upload --boot`. "
                "A CLI reutiliza um Network ID não-zero existente ou cria e persiste um novo. "
                "2. Copie o Network ID mostrado pela bridge (ou consulte com `ninalink-network-status`). "
                "3. Coloque o node a bateria em P0.21/NUS e grave "
                f"`{nl['node_prompt']}` substituindo 0x1234 pelo ID real da bridge. "
                f"O node opera em low power: Application/NDP fica desligado e o período padrão de contato/report "
                f"LoRa é {period} s; LLCC68 e MCU retornam ao caminho de baixo consumo entre contatos. "
                "A Bridge mantém RX LoRa contínuo e apresenta o node remoto ao Home Assistant via BLE/NDP."
            )
        return {"topic": "ninalink-provisioning", "answer": answer, "confidence": "high"}

    asks_ndp_key = (
        ("ndp" in q)
        and (("key" in q) or ("chave" in q))
        and not asks_network_vs_key
    )
    if asks_ndp_key:
        key = kb["home_assistant"]["ndp_owner_key"]
        if english:
            answer = (
                f"NDP protection is optional. Through the physical P0.21/NUS owner plane, run "
                f"`{key['generate_command']}` to generate/persist a {key['size_bits']}-bit key. "
                f"Copy the returned `{key['format']}` value into Home Assistant's "
                f"`{key['home_assistant_field']}` field. It can be read again with "
                f"`{key['read_command']}` through the physical owner plane."
            )
        else:
            answer = (
                f"A proteção NDP é opcional. Pelo plano físico P0.21/NUS, execute "
                f"`{key['generate_command']}` para gerar/persistir uma chave de {key['size_bits']} bits. "
                f"Copie o valor `{key['format']}` para o campo `{key['home_assistant_field']}` do Home Assistant. "
                f"Ela pode ser lida novamente com `{key['read_command']}` pelo plano físico de owner."
            )
        return {"topic": "ndp-owner-key", "answer": answer, "confidence": "high"}

    asks_ninalink_low_power = (
        ("ninalink" in q or ("lora" in q and "node" in q))
        and (
            ("low power" in q) or ("low-power" in q) or ("baixo consumo" in q)
            or ("battery" in q) or ("bateria" in q)
        )
    )
    if asks_ninalink_low_power:
        role = kb["home_assistant"]["roles"]["NINALINK_NODE"]
        period = int(role["default_report_period_s"])
        if english:
            answer = (
                f"A NinaLink Node is the battery-oriented HA role. Application/NDP is {role['application_ndp']}; "
                f"the node uses {role['transport']}. The default contact/report period is {period} s, and "
                f"{role['power']}. A NinaLink Bridge is different: it keeps LoRa RX active continuously and "
                "therefore belongs to a higher-power gateway class."
            )
        else:
            answer = (
                f"O NinaLink Node é o papel HA voltado para bateria. Application/NDP fica {role['application_ndp']}; "
                f"o node usa {role['transport']}. O período padrão de contato/report é {period} s e "
                f"{role['power']}. A Bridge NinaLink é diferente: mantém RX LoRa contínuo e por isso pertence "
                "a uma classe de gateway de maior consumo."
            )
        return {"topic": "ninalink-low-power", "answer": answer, "confidence": "high"}

    if re.search(r'lipo|li-po|bateria.*3[\.,]8|3[\.,]8v',q):
        e=kb['electrical']
        return {'topic':'power-input','answer':f"Não diretamente. A NINASENSE Rev {kb['board_revision']} aceita {e['supply_min_v']:.1f} a {e['supply_max_v']:.1f} V. Uma LiPo de 3,8 V nominal pode chegar a aproximadamente 4,2 V carregada, portanto excede o limite. Use um regulador de baixo consumo que mantenha a alimentação em até 3,6 V.", 'confidence':'high'}
    if ('lora' in q and any(x in q for x in ('alcance','range','distancia','distance'))):
        r=kb['radio']['lora']; g=kb['radio']['open_field_range']
        return {'topic':'lora-range','answer':f"O {r} não tem um alcance máximo fixo. Em campo aberto, uma referência de engenharia para uma instalação bem feita é cerca de {g['engineering_typical_km'][0]}–{g['engineering_typical_km'][1]} km; distâncias maiores podem ser possíveis com linha de visada e antenas adequadas. O valor real depende de antena, altura, SF, BW, potência, ruído, zona de Fresnel e limites regulatórios, portanto deve ser validado por link budget e teste de campo.", 'confidence':'medium', 'not_a_guarantee':True}
    if ('cr2032' in q and any(x in q for x in ('tracking','rastreamento','seguimiento'))):
        m=kb['power_models']; cell=m['cr2032']; tr=m['tracking_2s']
        interval=re.search(r'(\d+(?:[\.,]\d+)?)\s*(?:s|segundos?)',q)
        sec=float(interval.group(1).replace(',','.')) if interval else 2.0
        if abs(sec-2.0)>1e-9:
            return {'topic':'battery-life','answer':f"A base local só possui uma medição/proxy validada para advertising/tracking em 2 s. Para {sec:g} s eu não vou extrapolar automaticamente sem um modelo de corrente por intervalo. Meça a corrente média desse perfil ou adicione-a à base.", 'confidence':'high'}
        uA=float(tr['current_uA']); nominal=_years(cell['nominal_capacity_mah'],uA); cons=_years(cell['conservative_usable_capacity_mah'],uA)
        return {'topic':'battery-life','answer':f"Usando {uA:g} µA como proxy de corrente média para tracking/advertising a cada 2 s, uma CR2032 de {cell['nominal_capacity_mah']} mAh daria aproximadamente {nominal:.2f} anos no cálculo ideal. Usando {cell['conservative_usable_capacity_mah']} mAh úteis como estimativa conservadora, cerca de {cons:.2f} anos. Isto é uma estimativa, não garantia: pulsos de rádio, temperatura, marca da célula, tensão de corte, autodescarga e o payload real de tracking alteram o resultado. {tr['note']}", 'confidence':'medium', 'calculation':{'current_uA':uA,'ideal_years':round(nominal,2),'conservative_years':round(cons,2)}}
    if re.search(r'quais? (?:recursos|funcoes|funções)|o que .* oferece|vantagens|features|capabilities|what .* offer|recursos|ventajas|que .* ofrece',q):
        return {'topic':'features','answer':'Recursos principais: '+ '; '.join(kb['features'])+'. Vantagens: '+ '; '.join(kb['advantages'])+'.', 'confidence':'high'}
    m=re.search(r'(?:pino|pin|gpio|p0[\._]?)\s*(\d{1,2})',q)
    if m:
        key=f"P0.{int(m.group(1)):02d}"
        if key in kb['pins']:
            return {'topic':'pin','answer':f"{key}: {kb['pins'][key]}.", 'confidence':'high'}
    if any(x in q for x in ('tensao','voltagem','alimentacao','voltage','supply','voltaje','alimentacion')) or 'tensão' in question.lower():
        e=kb['electrical']; return {'topic':'power-input','answer':f"A faixa de alimentação registrada para a NINASENSE Rev {kb['board_revision']} é {e['supply_min_v']:.1f} a {e['supply_max_v']:.1f} V. {e['recommended_note']}", 'confidence':'high'}
    return {'topic':'unknown','answer':'Não encontrei uma resposta suficientemente segura na base local. A pergunta foi entendida como consulta de produto, mas precisa de uma entrada de conhecimento ou medição adicional; não vou inventar um valor.', 'confidence':'low'}
