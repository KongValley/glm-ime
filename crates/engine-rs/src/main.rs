//! glm-ime 引擎 — Rust 实现（协议 engine.v0，见 proto/engine.v0.md）。
//! stdio JSON Lines；诊断只走 stderr。与 engine-py 语义逐字节一致。

use serde_json::{json, Value};

/// 引擎文件日志：%LOCALAPPDATA%\glm-ime\logs\Engine-<pid>.log
/// （全事件日志：启动/init统计/panic/退出；按键热路径不写，保持零开销）
fn elog(msg: &str) {
    let mut dir = match std::env::var("LOCALAPPDATA") {
        Ok(d) => d,
        Err(_) => return,
    };
    dir.push_str("\\glm-ime\\logs");
    let _ = std::fs::create_dir_all(&dir);
    let path = format!("{dir}\\Engine-{}.log", std::process::id());
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&path) {
        use std::io::Write;
        let _ = writeln!(f, "{msg}");
    }
}
use std::collections::HashMap;
use std::io::{BufRead, BufWriter, Write};
use std::path::PathBuf;

#[derive(Clone)]
struct Word {
    flat: String, // py 去空格
    abbr: String, // 首字母缩写（简拼）
    text: String,
    freq: i64,
}

fn builtin_path(exe_dir: &std::path::Path) -> PathBuf {
    // 发布形态：exe 同目录；开发形态：CARGO_MANIFEST_DIR
    let p = exe_dir.join("lexicon.json");
    if p.exists() {
        p
    } else {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("lexicon.json")
    }
}

fn load_lexicon(user_dir: &str, exe_dir: &std::path::Path) -> Vec<Word> {
    let src = if !user_dir.is_empty() {
        let p = std::path::Path::new(user_dir).join("lexicon.json");
        if p.exists() {
            p
        } else {
            builtin_path(exe_dir)
        }
    } else {
        builtin_path(exe_dir)
    };
    let data: Value =
        serde_json::from_str(&std::fs::read_to_string(src).expect("lexicon.json unreadable"))
            .expect("lexicon.json invalid");
    data["words"]
        .as_array()
        .expect("words missing")
        .iter()
        .map(|w| Word {
            flat: w["py"].as_str().unwrap_or("").replace(' ', ""),
            abbr: w["ab"].as_str().unwrap_or("").to_string(),
            text: w["text"].as_str().unwrap_or("").to_string(),
            freq: w["freq"].as_i64().unwrap_or(0),
        })
        .collect()
}

fn lookup(words: &[Word], raw: &str) -> Vec<String> {
    let out = lookup_exact(words, raw);
    if !out.is_empty() {
        return out;
    }
    // 简拼回退（B19）：全拼无果时按首字母缩写匹配（含前缀），如 "sd" -> 是的/速度
    if raw.is_empty() || !raw.is_ascii() {
        return out;
    }
    let mut score: std::collections::HashMap<&str, i64> = std::collections::HashMap::new();
    for w in words {
        let s = if w.abbr == raw {
            w.freq + 10000
        } else if w.abbr.starts_with(raw) {
            w.freq - 500   // 简拼前缀略降权（低于精确/全拼）
        } else {
            continue;
        };
        score
            .entry(w.text.as_str())
            .and_modify(|old| {
                if s > *old {
                    *old = s;
                }
            })
            .or_insert(s);
    }
    let mut v: Vec<(&str, i64)> = score.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
    v.truncate(9);
    v.into_iter().map(|(t, _)| t.to_string()).collect()
}

/// 全拼（精确+前缀）查询
fn lookup_exact(words: &[Word], raw: &str) -> Vec<String> {
    let mut score: std::collections::HashMap<&str, i64> = std::collections::HashMap::new();
    for w in words {
        let s = if w.flat == raw {
            w.freq + 10000
        } else if !raw.is_empty() && w.flat.starts_with(raw) {
            w.freq
        } else {
            continue;
        };
        score
            .entry(w.text.as_str())
            .and_modify(|old| {
                if s > *old {
                    *old = s;
                }
            })
            .or_insert(s);
    }
    let mut v: Vec<(&str, i64)> = score.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
    v.truncate(9);
    v.into_iter().map(|(t, _)| t.to_string()).collect()
}

struct Session {
    raw: String,
    cands: Vec<String>,
    user_dict_path: Option<std::path::PathBuf>,
}

impl Session {
    fn new() -> Self {
        Session { raw: String::new(), cands: vec![], user_dict_path: None }
    }

    /// 自学习：非 ASCII 提交 + 编码 → 内存热合并 + user_lexicon.json 落盘
    fn learn(&self, words_slot: &std::sync::Mutex<Vec<Word>>) {
        let text = match self.cands.first() {
            Some(t) if !t.is_empty() && t.chars().any(|c| !c.is_ascii()) => t.clone(),
            _ => return,
        };
        let code = self.raw.clone();
        if code.is_empty() { return; }
        // 热合并：当前词库
        {
            let mut w = words_slot.lock().unwrap();
            w.push(Word { flat: code.clone(), abbr: code.clone(), text: text.clone(), freq: 10000 });
        }
        // 落盘
        if let Some(path) = &self.user_dict_path {
            let mut ud: serde_json::Value = std::fs::read_to_string(path)
                .ok()
                .and_then(|s| serde_json::from_str(&s).ok())
                .unwrap_or_else(|| serde_json::json!({}));
            if !ud.is_object() { ud = serde_json::json!({}); }
            if ud["words"].is_null() { ud["words"] = serde_json::json!({}); }
            let words = ud["words"].as_object_mut().expect("words map");
            let entries = words.entry(code.clone()).or_insert_with(|| serde_json::json!([]));
            let arr = entries.as_array_mut().expect("entries array");
            let mut found = false;
            for e in arr.iter_mut() {
                if e[0].as_str() == Some(text.as_str()) {
                    let f = e[1].as_i64().unwrap_or(0) + 1;
                    *e = serde_json::json!([text, f]);
                    found = true;
                    break;
                }
            }
            if !found {
                arr.push(serde_json::json!([text, 1]));
                if arr.len() > 9 {
                    arr.sort_by(|a, b| b[1].as_i64().unwrap_or(0).cmp(&a[1].as_i64().unwrap_or(0)));
                    arr.truncate(9);
                }
            }
            if path.parent().map(|d| std::fs::create_dir_all(d)).transpose().is_ok() {
                let _ = std::fs::write(path, serde_json::to_string_pretty(&ud).unwrap_or_default());
            }
        }
    }

    fn acts(&self) -> Vec<Value> {
        let mut acts = vec![];
        if !self.raw.is_empty() {
            acts.push(json!({"action":"composition","text":self.raw,"cursor":self.raw.chars().count()}));
            // 组词中总是显式发送候选（含空列表）：空列表 = 关闭候选窗。
            // 若省略空候选，TIP 无法感知"候选消失"，候选窗将残留显示。
            acts.push(json!({"action":"candidates","list":self.cands}));
        }
        acts
    }

    fn clear(&mut self) -> Vec<Value> {
        self.raw.clear();
        self.cands.clear();
        vec![
            json!({"action":"composition","text":"","cursor":0}),
            json!({"action":"candidates","list":[]}),
        ]
    }

    fn step(&mut self, words_slot: &std::sync::Mutex<Vec<Word>>, code: u32, ch: &str) -> (bool, Vec<Value>) {
        let is_lower = ch.len() == 1 && ch.as_bytes()[0].is_ascii_lowercase();
        if is_lower {
            self.raw.push_str(ch);
            self.cands = { let w = words_slot.lock().unwrap(); lookup(&w, &self.raw) };
            return (true, self.acts());
        }
        match code {
            32 => {
                // space：有候选→commit 第 1 个；无候选有缓冲→commit 原始缓冲
                if self.raw.is_empty() {
                    return (false, vec![]);
                }
                let text = self.cands.first().cloned().unwrap_or_else(|| self.raw.clone());
                self.learn(words_slot);
                let mut acts = vec![json!({"action":"commit","text":text})];
                acts.extend(self.clear());
                (true, acts)
            }
            c @ 0x31..=0x39 => {
                let idx = (c - 0x31) as usize;
                if let Some(text) = self.cands.get(idx).cloned() {
                    self.learn(words_slot);
                    let mut acts = vec![json!({"action":"commit","text":text})];
                    acts.extend(self.clear());
                    (true, acts)
                } else {
                    (false, vec![])
                }
            }
            8 => {
                if self.raw.is_empty() {
                    return (false, vec![]);
                }
                self.raw.pop();
                self.cands = if self.raw.is_empty() { vec![] } else { let w = words_slot.lock().unwrap(); lookup(&w, &self.raw) };
                (true, self.acts())
            }
            13 => {
                if self.raw.is_empty() {
                    return (false, vec![]);
                }
                let text = self.raw.clone();
                let mut acts = vec![json!({"action":"commit","text":text})];
                acts.extend(self.clear());
                (true, acts)
            }
            27 => {
                if self.raw.is_empty() && self.cands.is_empty() {
                    return (false, vec![]);
                }
                (true, self.clear())
            }
            _ => (false, vec![]),
        }
    }
}

fn handle(msg: Value, words: &std::sync::Arc<std::sync::Mutex<Vec<Word>>>, session: &mut Session) -> Option<Value> {
    match msg["op"].as_str().unwrap_or("") {
        "init" => {
            let user_dir = msg["user_dir"].as_str().unwrap_or("");
            let exe_dir = std::env::current_exe()
                .ok()
                .and_then(|p| p.parent().map(|p| p.to_path_buf()))
                .unwrap_or_default();
            let mut w = words.lock().unwrap();
            *w = load_lexicon(user_dir, &exe_dir);
            elog(&format!("init: words={} user_dir={:?}", w.len(), user_dir));
            if !user_dir.is_empty() {
                session.user_dict_path = Some(std::path::Path::new(user_dir).join("user_lexicon.json"));
                // 启动加载用户词典
                let up = std::path::Path::new(user_dir).join("user_lexicon.json");
                if let Ok(txt) = std::fs::read_to_string(&up) {
                    if let Ok(ud) = serde_json::from_str::<serde_json::Value>(&txt) {
                        if let Some(obj) = ud["words"].as_object() {
                            for (flat, entries) in obj {
                                if let Some(arr) = entries.as_array() {
                                    for e in arr {
                                        w.push(Word {
                                            flat: flat.clone(),
                                            abbr: flat.clone(),
                                            text: e[0].as_str().unwrap_or("").to_string(),
                                            freq: e[1].as_i64().unwrap_or(1),
                                        });
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Some(json!({"ok":true,"info":{"name":"glm-ime-rs","version":"0.2"}}))
        }
        "reset" => {
            *session = Session::new();
            Some(json!({"ok":true,"consumed":true,"actions":[]}))
        }
        "key" => {
            let code = msg["code"].as_u64().unwrap_or(0) as u32;
            let ch = msg["char"].as_str().unwrap_or("");
            let (consumed, actions) = session.step(words, code, ch);
            Some(json!({"ok":true,"consumed":consumed,"actions":actions}))
        }
        "shutdown" => None,
        other => Some(json!({"ok":false,"error":format!("unknown op {other:?}")})),
    }
}

fn main() {
    // 崩溃追溯：panic 全量落盘（含位置与消息）
    std::panic::set_hook(Box::new(|info| {
        elog(&format!("PANIC: {info}"));
    }));
    elog(&format!("engine start pid={} version={}", std::process::id(), env!("CARGO_PKG_VERSION")));
    let words: std::sync::Arc<std::sync::Mutex<Vec<Word>>> = std::sync::Arc::new(std::sync::Mutex::new(vec![]));
    let mut session = Session::new();
    let stdin = std::io::stdin();
    let stdout = std::io::stdout();
    let mut out = std::io::BufWriter::new(stdout.lock());
    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        if line.trim().is_empty() {
            continue;
        }
        let reply = match serde_json::from_str::<Value>(&line) {
            Ok(msg) => match handle(msg, &words, &mut session) {
                None => {
                    elog("engine exit (shutdown)");
                    let _ = writeln!(out, r#"{{"ok":true}}"#);
                    let _ = out.flush();
                    std::process::exit(0);
                }
                Some(r) => r,
            },
            Err(e) => {
                elog(&format!("bad request: {e}"));
                json!({"ok":false,"error":e.to_string()})
            }
        };
        let _ = writeln!(out, "{reply}");
        let _ = out.flush();
    }
    elog("engine exit (stdin EOF)");
}
