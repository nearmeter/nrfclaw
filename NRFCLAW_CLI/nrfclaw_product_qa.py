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
