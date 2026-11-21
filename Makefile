CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

SRC := src/types.cpp src/position.cpp src/notation.cpp src/movegen.cpp src/game.cpp src/mate.cpp
HDR := src/types.h src/position.h src/notation.h src/movegen.h src/game.h src/mate.h
BIN := daishogi
UNIT := tests/unit_tests
REFGEN := tools/refgen/target/release/refgen
PY ?= python3

AZSRC := src/az/encode.cpp src/az/mcts.cpp src/az/selfplay.cpp src/az/eval_client.cpp src/az/main.cpp
AZHDR := src/az/encode.h src/az/mcts.h src/az/selfplay.h src/az/eval_client.h
AZBIN := daishogi-az

all: $(BIN)

$(BIN): $(SRC) src/main.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) src/main.cpp

# AlphaZero self-play worker / match player (see az/README.md).
$(AZBIN): $(SRC) $(AZSRC) $(HDR) $(AZHDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(AZSRC)

$(UNIT): $(SRC) tests/unit_tests.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(SRC) tests/unit_tests.cpp

# Mate solver against brute force (~2 minutes; see tests/mate_test.cpp).
MATETEST := tests/mate_test
$(MATETEST): $(SRC) tests/mate_test.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) tests/mate_test.cpp

mate-test: $(MATETEST)
	./$(MATETEST) tests/data/mate_positions.txt

# AlphaZero tests (see az/README.md). AZPY must have NumPy and PyTorch;
# the pipe and train tests start the eval server (GPU if available).
# Usage: make az-test AZPY=~/venv/bin/python
AZPY ?= $(PY)
AZTESTS := tests/az/search_value_test tests/az/selection_test tests/az/selfplay_game_test
AZLIB := $(SRC) src/az/encode.cpp src/az/mcts.cpp src/az/selfplay.cpp

tests/az/%_test: tests/az/%_test.cpp $(AZLIB) $(HDR) $(AZHDR)
	$(CXX) $(CXXFLAGS) -o $@ $(AZLIB) $<

az-test: $(BIN) $(AZBIN) $(AZTESTS) $(MATETEST)
	./$(MATETEST) tests/data/mate_positions.txt
	for t in $(AZTESTS); do ./$$t || exit 1; done
	$(AZPY) -m unittest tests/az/buffer_test.py tests/az/legal_policy_test.py \
	  tests/az/eval_output_test.py tests/az/inference_batcher_test.py \
	  tests/az/optimizers_test.py tests/az/selfplay_pool_test.py tests/az/tournament_test.py
	$(AZPY) tests/az/encode_test.py
	$(AZPY) tests/az/selfplay_test.py
	$(AZPY) tests/az/selfplay_test.py --full-prob 0.25
	$(AZPY) tests/az/selfplay_test.py --mate
	$(AZPY) tests/az/mcts_test.py
	$(AZPY) tests/az/pipe_test.py
	$(AZPY) tests/az/train_test.py

# Reference data generator (Rust; drives ../haitaka-variants, unmodified).
$(REFGEN): tools/refgen/src/main.rs tools/refgen/Cargo.toml
	cd tools/refgen && cargo build --release --offline

# Fast tests: C++ unit tests + comparison against the committed reference
# corpus (no Rust toolchain needed).
test: $(BIN) $(UNIT)
	./$(UNIT)
	$(PY) tests/compare.py --binary ./$(BIN) tests/data/*.jsonl.gz
	$(PY) tests/replay_games.py --binary ./$(BIN) tests/data/*.jsonl.gz

# Regenerate the committed corpus from the reference implementation.
corpus: $(REFGEN)
	$(PY) tests/make_corpus.py --refgen $(REFGEN) --out tests/data

# Fresh, larger comparison straight against the reference (needs Rust).
# Usage: make reftest SEED=7 GAMES=200
SEED ?= 1000
GAMES ?= 100
reftest: $(BIN) $(REFGEN)
	$(PY) tests/live_compare.py --binary ./$(BIN) --refgen $(REFGEN) --seed $(SEED) --games $(GAMES)

# Break one rule at a time and check that the corpus catches every mutant.
mutation:
	$(PY) tests/mutation_check.py

# Random vs random. Usage: make play SEED=42 GAMES=3 MAXPLIES=2000
MAXPLIES ?= 0
play: $(BIN)
	./$(BIN) play --seed $(SEED) --games $(GAMES) --max-plies $(MAXPLIES) --quiet

clean:
	rm -f $(BIN) $(AZBIN) $(UNIT) $(AZTESTS) $(MATETEST) tests/mismatches.jsonl
	rm -rf tests/__pycache__ tests/az/__pycache__ az/__pycache__

.PHONY: all test az-test mate-test corpus reftest mutation play clean
