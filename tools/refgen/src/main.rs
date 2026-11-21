//! Reference data generator for dai shogi (Shogitter rule 49).
//!
//! Drives the `haitaka-variants` implementation (feature `daishogi`) without
//! modifying it, and emits JSON lines in the reference's own wire format.
//! The Python test harness converts these to our SFEN/USI notation.
//!
//! Deliberate differences (docs/RULES.md, discrepancies 5 and 9). Our
//! engine follows lishogi / JCSA in two lion-versus-lion details where the
//! reference does not. `deviations()` derives both sets from the reference
//! itself, so they are exact for every position:
//!
//!   forbidden  A lion takes a pawn or go-between, then a lion that is not
//!              adjacent to its start square. The captured piece no longer
//!              blocks a defender's line: protection is judged with the
//!              first-step square emptied as well. For each such reference
//!              move we ask the reference again with that piece removed and
//!              flag the move if it is then illegal.
//!   added      A lion may always take a lion adjacent to its start square,
//!              also on the second step of a two-step move (through an empty
//!              square or a pawn / go-between). The reference applies the
//!              adjacency exemption to single steps only. Every such move
//!              missing from the reference's list is added. Its result is
//!              the single-step capture played on the board with the
//!              first-step victim removed (`play_added`).
//!
//! "Our moves" are the reference's moves minus `forbidden` plus `added`.
//! `games` picks from our moves, `perft` counts them, and `replay` accepts
//! them. An added move captures a lion, so no earlier position can recur;
//! restarting the history there (as `play_added` does) cannot change any
//! repetition result.
//!
//! Commands (all output is one JSON object per line on stdout):
//!
//!   refgen games --seed S --games N --max-plies P --sample K --capture-bias B
//!       Plays N reproducible games. Every K-th position (and the final one)
//!       is emitted as {"type":"position",...} with the reference's complete
//!       legal move list for that position (evaluated as a fresh root, i.e.
//!       without repetition history). At the end of every game a
//!       {"type":"game",...} record carries the full move list and outcome.
//!       With probability B a ply picks uniformly among capturing moves (if
//!       any) instead of among all moves; this reaches sparse positions and
//!       lion fights much sooner than uniform random play. Moves are picked
//!       from our moves. Position records carry the reference's "moves" plus
//!       "forbidden" and "added" (see above).
//!
//!   refgen moves [--transitions]    stdin: one wire position per line.
//!       Emits {"moves":[...],"outcome":...} or {"error":"..."}. With
//!       --transitions, "next" holds the position after each move.
//!       "forbidden" and "added" as above; with --transitions,
//!       "added_next" holds the position after each added move.
//!
//!   refgen replay     stdin: {"start": wire|null, "moves": [...]} per line.
//!       Plays the moves from the start position (null = initial position)
//!       with full history, so repetition rules apply. Emits
//!       {"ok":true,"plies":n,"outcome":...,"final":wire} or
//!       {"ok":false,"plies":n,"error":...} at the first move that is not
//!       one of our moves. "added" counts the added moves played.
//!
//!   refgen perft --depth D      stdin: one wire position per line.
//!       Emits {"depth":D,"nodes":n} over our moves (the reference's own
//!       count is "reference_nodes" when it differs).
//!
//!   refgen startpos   prints the initial wire position.
//!
//!   refgen bench [--depth D] [--games N] [--max-plies M]
//!       Times the reference's own perft from the start position and N
//!       uniform random games, for speed comparisons with `daishogi`.

use haitaka::extended::compiled::{CompiledGame, start_position_json};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::io::{self, BufRead, Write};

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        // splitmix64
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
    fn chance(&mut self, p: f64) -> bool {
        ((self.next() >> 11) as f64 / (1u64 << 53) as f64) < p
    }
}

fn request(position: &Value) -> String {
    json!({"schemaVersion":3,"ruleId":49,"position":position,"history":null,"pending":null})
        .to_string()
}

fn wire(g: &CompiledGame) -> Value {
    serde_json::to_value(g.current().to_wire()).unwrap()
}

fn root_moves(g: &CompiledGame) -> Value {
    let root = CompiledGame::root(g.current().clone());
    serde_json::to_value(root.moves()).unwrap()
}

fn root_outcome(g: &CompiledGame) -> Value {
    serde_json::to_value(CompiledGame::root(g.current().clone()).outcome()).unwrap()
}

fn at(v: &Value) -> (u64, u64) {
    (v[0].as_u64().unwrap(), v[1].as_u64().unwrap())
}

fn pieces_by_square(position: &Value) -> HashMap<(u64, u64), (String, u64)> {
    position["pieces"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| {
            (at(&p["at"]), (p["piece"].as_str().unwrap().to_string(), p["color"].as_u64().unwrap()))
        })
        .collect()
}

const LIONS: [&str; 2] = ["bl", "dh"]; // lion, lion promoted from kirin
const PAWN_OR_GO_BETWEEN: [&str; 2] = ["aa", "cq"];

fn chebyshev(a: (u64, u64), b: (u64, u64)) -> u64 {
    a.0.abs_diff(b.0).max(a.1.abs_diff(b.1))
}

fn two_step(from: (u64, u64), mid: (u64, u64), to: (u64, u64)) -> Value {
    json!({"from": [from.0, from.1], "kind": "move", "path": [[mid.0, mid.1], [to.0, to.1]],
           "promotion": false, "schemaVersion": 3})
}

struct Deviations {
    forbidden: Vec<Value>,
    added: Vec<Value>,
}

fn without(position: &Value, sq: (u64, u64)) -> Value {
    let mut p = position.clone();
    p["pieces"].as_array_mut().unwrap().retain(|q| at(&q["at"]) != sq);
    p
}

/// The two deliberate differences for one position (see the header).
fn deviations(position: &Value, moves: &[Value]) -> Deviations {
    let side = position["sideToMove"].as_u64().unwrap();
    let board = pieces_by_square(position);
    let is = |sq: (u64, u64), kinds: &[&str], own: bool| {
        board.get(&sq).is_some_and(|(id, c)| kinds.contains(&id.as_str()) && (*c == side) == own)
    };
    let mut forbidden = vec![];
    for mv in moves {
        let Some(path) = mv["path"].as_array() else { continue };
        if path.len() != 2 {
            continue;
        }
        let (from, mid, to) = (at(&mv["from"]), at(&path[0]), at(&path[1]));
        if to == from || chebyshev(from, to) == 1 || !is(from, &LIONS, true)
            || !is(mid, &PAWN_OR_GO_BETWEEN, false) || !is(to, &LIONS, false)
        {
            continue;
        }
        let still_legal = CompiledGame::parse(&request(&without(position, mid)))
            .map(|g| g.moves().iter().any(|m| serde_json::to_value(m).unwrap() == *mv))
            .unwrap_or(false);
        if !still_legal {
            forbidden.push(mv.clone());
        }
    }
    let mut added = vec![];
    // No moves at all means the game is over (a side has no royal): nothing
    // to add, in either engine.
    if !moves.is_empty() && board.values().any(|(id, c)| LIONS.contains(&id.as_str()) && *c != side) {
        for (&from, _) in board.iter().filter(|(sq, _)| is(**sq, &LIONS, true)) {
            for (&to, _) in board.iter().filter(|(sq, _)| is(**sq, &LIONS, false)) {
                if chebyshev(from, to) != 1 {
                    continue;
                }
                for x in from.0.max(2) - 1..=(from.0 + 1).min(15) {
                    for y in from.1.max(2) - 1..=(from.1 + 1).min(15) {
                        let mid = (x, y);
                        if mid == from || mid == to || chebyshev(mid, to) != 1 {
                            continue;
                        }
                        if board.contains_key(&mid) && !is(mid, &PAWN_OR_GO_BETWEEN, false) {
                            continue;
                        }
                        let mv = two_step(from, mid, to);
                        if !moves.contains(&mv) {
                            added.push(mv);
                        }
                    }
                }
            }
        }
        added.sort_by_key(|v| v.to_string());
    }
    Deviations { forbidden, added }
}

/// The position after an added move: the single-step capture of the
/// adjacent lion, on the board with the first-step victim removed. The new
/// game has no history (see the header for why that is exact).
fn play_added(position: &Value, mv: &Value) -> CompiledGame {
    let mid = at(&mv["path"][0]);
    let to = &mv["path"][1];
    let mut g = CompiledGame::parse(&request(&without(position, mid))).unwrap();
    let single = g
        .moves()
        .into_iter()
        .find(|m| {
            let v = serde_json::to_value(m).unwrap();
            v["from"] == mv["from"] && v["path"] == json!([to])
        })
        .expect("single-step capture of an adjacent lion is legal in the reference");
    g.play(single).unwrap();
    g
}

fn move_values(g: &CompiledGame) -> Vec<Value> {
    g.moves().iter().map(|m| serde_json::to_value(m).unwrap()).collect()
}

/// Our moves in a game position, each with the game after it.
fn our_children(g: &CompiledGame) -> Vec<(Value, CompiledGame)> {
    let values = move_values(g);
    let w = wire(g);
    let dev = deviations(&w, &values);
    let mut out = vec![];
    for v in values {
        if dev.forbidden.contains(&v) {
            continue;
        }
        let mut next = g.clone();
        next.play(serde_json::from_value(v.clone()).unwrap()).unwrap();
        out.push((v, next));
    }
    for v in dev.added {
        let next = play_added(&w, &v);
        out.push((v, next));
    }
    out
}

/// Perft over our moves.
fn perft_ours(g: &CompiledGame, depth: u8) -> u64 {
    if depth == 0 {
        return 1;
    }
    if depth == 1 {
        let values = move_values(g);
        let dev = deviations(&wire(g), &values);
        return (values.len() - dev.forbidden.len() + dev.added.len()) as u64;
    }
    our_children(g).iter().map(|(_, next)| perft_ours(next, depth - 1)).sum()
}

fn replay(line: &Value) -> Value {
    let start = if line["start"].is_null() {
        start_position_json()
    } else {
        request(&line["start"])
    };
    let mut g = match CompiledGame::parse(&start) {
        Ok(g) => g,
        Err(e) => return json!({"ok": false, "plies": 0, "error": e}),
    };
    let moves = line["moves"].as_array().cloned().unwrap_or_default();
    let mut added = 0;
    for (i, mv) in moves.iter().enumerate() {
        let parsed = match serde_json::from_value(mv.clone()) {
            Ok(m) => m,
            Err(e) => return json!({"ok": false, "plies": i, "error": e.to_string()}),
        };
        let w = wire(&g);
        let values = move_values(&g);
        let dev = deviations(&w, &values);
        if dev.forbidden.contains(mv) {
            return json!({"ok": false, "plies": i,
                          "error": "move forbidden by our rules (reference-legal)"});
        }
        if dev.added.contains(mv) {
            g = play_added(&w, mv);
            added += 1;
            continue;
        }
        if let Err(e) = g.play(parsed) {
            return json!({"ok": false, "plies": i, "error": e});
        }
    }
    json!({"ok": true, "plies": moves.len(), "added": added, "outcome": g.outcome(),
           "final": wire(&g)})
}

fn arg(args: &[String], name: &str, default: &str) -> String {
    args.iter()
        .position(|a| a == name)
        .and_then(|i| args.get(i + 1).cloned())
        .unwrap_or_else(|| default.to_string())
}

fn occupancy(position: &Value) -> HashMap<(u64, u64), u64> {
    let mut m = HashMap::new();
    for p in position["pieces"].as_array().unwrap() {
        let at = p["at"].as_array().unwrap();
        m.insert(
            (at[0].as_u64().unwrap(), at[1].as_u64().unwrap()),
            p["color"].as_u64().unwrap(),
        );
    }
    m
}

fn is_capture(mv: &Value, occ: &HashMap<(u64, u64), u64>, side: u64) -> bool {
    mv["path"].as_array().is_some_and(|path| {
        path.iter().any(|sq| {
            let s = sq.as_array().unwrap();
            occ.get(&(s[0].as_u64().unwrap(), s[1].as_u64().unwrap()))
                .is_some_and(|&c| c != side)
        })
    })
}

fn games(args: &[String]) {
    let seed: u64 = arg(args, "--seed", "1").parse().unwrap();
    let n: usize = arg(args, "--games", "1").parse().unwrap();
    let max_plies: usize = arg(args, "--max-plies", "1000").parse().unwrap();
    let sample: usize = arg(args, "--sample", "1").parse().unwrap();
    let bias: f64 = arg(args, "--capture-bias", "0").parse().unwrap();
    let out = io::stdout();
    let mut out = out.lock();
    for game_index in 0..n {
        let game_seed = seed.wrapping_mul(1_000_003).wrapping_add(game_index as u64);
        let mut rng = Rng(game_seed);
        let mut g = CompiledGame::parse(&start_position_json()).unwrap();
        let mut played: Vec<Value> = vec![];
        let mut last: Value = Value::Null;
        let mut ply = 0;
        loop {
            let outcome = g.outcome();
            let finished = outcome.kind != "ongoing" || ply >= max_plies;
            let w = wire(&g);
            let values = move_values(&g);
            let dev = deviations(&w, &values);
            if ply % sample == 0 || finished {
                writeln!(
                    out,
                    "{}",
                    json!({"type":"position","seed":game_seed,"game":game_index,"ply":ply,
                           "lastMove":last,"position":w,"moves":root_moves(&g),
                           "forbidden":dev.forbidden,"added":dev.added,
                           "outcome":root_outcome(&g)})
                )
                .unwrap();
            }
            if finished {
                writeln!(
                    out,
                    "{}",
                    json!({"type":"game","seed":game_seed,"game":game_index,"plies":ply,
                           "captureBias":bias,"cutoff":outcome.kind == "ongoing",
                           "moves":played,"outcome":outcome})
                )
                .unwrap();
                break;
            }
            // Pick among our moves, so the game replays in our engine.
            let values: Vec<Value> = values
                .into_iter()
                .filter(|v| !dev.forbidden.contains(v))
                .chain(dev.added.iter().cloned())
                .collect();
            let mut pick = rng.below(values.len());
            if bias > 0.0 && rng.chance(bias) {
                let side = w["sideToMove"].as_u64().unwrap();
                let occ = occupancy(&w);
                let caps: Vec<usize> = (0..values.len())
                    .filter(|&i| is_capture(&values[i], &occ, side))
                    .collect();
                if !caps.is_empty() {
                    pick = caps[rng.below(caps.len())];
                }
            }
            if dev.added.contains(&values[pick]) {
                g = play_added(&w, &values[pick]);
            } else {
                g.play(serde_json::from_value(values[pick].clone()).unwrap()).unwrap();
            }
            last = values[pick].clone();
            played.push(last.clone());
            ply += 1;
        }
    }
}

fn per_line(f: impl Fn(&Value) -> Value) {
    let stdin = io::stdin();
    let out = io::stdout();
    let mut out = out.lock();
    for line in stdin.lock().lines() {
        let line = line.unwrap();
        if line.trim().is_empty() {
            continue;
        }
        let v: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(e) => json!({"error": e.to_string()}),
        };
        let r = if v.get("error").is_some() { v } else { f(&v) };
        writeln!(out, "{r}").unwrap();
        out.flush().unwrap();
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("games") => games(&args),
        Some("startpos") => {
            let g = CompiledGame::parse(&start_position_json()).unwrap();
            println!("{}", wire(&g));
        }
        Some("moves") => {
            let transitions = args.iter().any(|a| a == "--transitions");
            per_line(|pos| match CompiledGame::parse(&request(pos)) {
                Ok(g) => {
                    let moves = g.moves();
                    let values = move_values(&g);
                    let dev = deviations(&wire(&g), &values);
                    let mut r = json!({"moves": serde_json::to_value(&moves).unwrap(),
                                       "forbidden": dev.forbidden, "added": dev.added,
                                       "outcome": g.outcome()});
                    if transitions {
                        let next: Vec<Value> = moves
                            .iter()
                            .map(|&m| {
                                let mut n = g.clone();
                                n.play(m).unwrap();
                                wire(&n)
                            })
                            .collect();
                        r["next"] = Value::Array(next);
                        let w = wire(&g);
                        let added_next: Vec<Value> = r["added"]
                            .as_array()
                            .unwrap()
                            .iter()
                            .map(|m| wire(&play_added(&w, m)))
                            .collect();
                        r["added_next"] = Value::Array(added_next);
                    }
                    r
                }
                Err(e) => json!({"error": e}),
            })
        }
        Some("replay") => per_line(replay),
        Some("bench") => {
            // Timing of the reference's own code only: perft from the start
            // position (bulk-counted at the last ply, as `daishogi perft`),
            // then uniform random games (moves + play + outcome per ply).
            let depth: u8 = arg(&args, "--depth", "3").parse().unwrap();
            let games: usize = arg(&args, "--games", "20").parse().unwrap();
            let max_plies: usize = arg(&args, "--max-plies", "2000").parse().unwrap();
            let start = CompiledGame::parse(&start_position_json()).unwrap();
            let t = std::time::Instant::now();
            let nodes = start.clone().perft(depth);
            let s = t.elapsed().as_secs_f64();
            println!("perft {depth}: {nodes} nodes in {s:.3}s");
            let mut rng = Rng(1);
            let (mut plies, t) = (0usize, std::time::Instant::now());
            for _ in 0..games {
                let mut g = start.clone();
                for _ in 0..max_plies {
                    if g.outcome().kind != "ongoing" {
                        break;
                    }
                    let moves = g.moves();
                    g.play(moves[rng.below(moves.len())]).unwrap();
                    plies += 1;
                }
            }
            let s = t.elapsed().as_secs_f64();
            println!("random play: {games} games, {plies} plies in {s:.3}s = {:.0} plies/s", plies as f64 / s);
        }
        Some("perft") => {
            let depth: u8 = arg(&args, "--depth", "1").parse().unwrap();
            per_line(|pos| match CompiledGame::parse(&request(pos)) {
                Ok(mut g) => {
                    let nodes = perft_ours(&g, depth);
                    let reference = g.perft(depth);
                    let mut r = json!({"depth": depth, "nodes": nodes});
                    if reference != nodes {
                        r["reference_nodes"] = json!(reference);
                    }
                    r
                }
                Err(e) => json!({"error": e}),
            })
        }
        _ => {
            eprintln!("usage: refgen games|moves|replay|perft|startpos [options]");
            std::process::exit(2);
        }
    }
}
