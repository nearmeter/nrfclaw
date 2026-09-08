from __future__ import annotations
import hashlib, json, sqlite3, time, unicodedata, re
from pathlib import Path

SCHEMA='''
CREATE TABLE IF NOT EXISTS prompt_cache(
 key TEXT PRIMARY KEY,
 normalized_prompt TEXT NOT NULL,
 compiler_version TEXT NOT NULL,
 kb_sha256 TEXT NOT NULL,
 semantic_ok INTEGER NOT NULL,
 payload_json TEXT NOT NULL,
 created_at INTEGER NOT NULL,
 hits INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS prompt_cache_norm ON prompt_cache(normalized_prompt);
'''

def normalize_prompt(text:str)->str:
    t=unicodedata.normalize('NFKC',text).casefold()
    t=re.sub(r'\s+',' ',t).strip()
    t=re.sub(r'\s*([,.;:!?])\s*',r'\1 ',t).strip()
    return t

def cache_key(prompt:str, compiler_version:str, kb_sha256:str)->str:
    raw='\0'.join((normalize_prompt(prompt),str(compiler_version),str(kb_sha256))).encode()
    return hashlib.sha256(raw).hexdigest()

class SemanticCache:
    def __init__(self,path):
        self.path=Path(path); self.path.parent.mkdir(parents=True,exist_ok=True)
        self.db=sqlite3.connect(self.path)
        self.db.executescript(SCHEMA); self.db.commit()
    def get(self,prompt,compiler_version,kb_sha256):
        k=cache_key(prompt,compiler_version,kb_sha256)
        row=self.db.execute('SELECT semantic_ok,payload_json FROM prompt_cache WHERE key=?',(k,)).fetchone()
        if not row:return None
        self.db.execute('UPDATE prompt_cache SET hits=hits+1 WHERE key=?',(k,));self.db.commit()
        return {'semantic_ok':bool(row[0]),'payload':json.loads(row[1]),'key':k}
    def put(self,prompt,compiler_version,kb_sha256,semantic_ok,payload):
        k=cache_key(prompt,compiler_version,kb_sha256)
        self.db.execute('INSERT OR REPLACE INTO prompt_cache(key,normalized_prompt,compiler_version,kb_sha256,semantic_ok,payload_json,created_at,hits) VALUES(?,?,?,?,?,?,?,COALESCE((SELECT hits FROM prompt_cache WHERE key=?),0))',
            (k,normalize_prompt(prompt),str(compiler_version),str(kb_sha256),int(bool(semantic_ok)),json.dumps(payload,ensure_ascii=False,separators=(',',':')),int(time.time()),k))
        self.db.commit(); return k
    def stats(self):
        total,hits,ok=self.db.execute('SELECT COUNT(*),COALESCE(SUM(hits),0),COALESCE(SUM(semantic_ok),0) FROM prompt_cache').fetchone()
        return {'entries':total,'hits':hits,'semantic_ok_entries':ok}
