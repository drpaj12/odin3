# Architecture Decision Records

Source: `docs/DESIGN.md` §2 ("Decisions already made"), spec v0.2, 2026-10-08. These records
encode the architect's rules; where a record says "(interpretation - confirm)" the text goes
beyond the spec's literal wording.

| ID | Title | Status | Date | Summary |
|---|---|---|---|---|
| [D1](D1.md) | One IR, two granularities | Accepted | 2026-10-08 | Word-level and bit-level views of one IR, moved by `lower`/`raise`. |
| [D2](D2.md) | Owned Verilog-2005 grammar; SV/VHDL via adapters | Accepted | 2026-10-08 | Bison/Flex Verilog-2005; slang for SV; GHDL subprocess for VHDL. |
| [D3](D3.md) | MIT; subprocess tools except linked ABC | Accepted | 2026-10-08 | Core stays MIT; GHDL, Yosys, Quartus, VPR are subprocesses; `libabc` is linked. |
| [D4](D4.md) | Simulator and equivalence checking | Accepted | 2026-10-08 | Built-in netlist simulator plus ABC `cec`/`dsec` and Verilator checks. |
| [D5](D5.md) | Subgraph-isomorphism hard-block inference | Accepted | 2026-10-08 | IR-fragment patterns with a cost model; one matcher serves forward and RE. |
| [D6](D6.md) | Altera primitives first-class | Accepted | 2026-10-08 | Primitive library elaborates to generic cells; `bind`/`unbind` passes. |
| [D7](D7.md) | Not built on MLIR/CIRCT | Accepted | 2026-10-08 | Borrow ideas; provide `write_circt`, later `read_circt`. |
| [D8](D8.md) | Core in C++20 (v0.1) | Superseded by D8' | 2026-10-08 | Original language choice; superseded, rationale not recorded in the spec. |
| [D8'](D8-prime.md) | Core in structured C17, public C ABI | Accepted | 2026-10-08 | C17 core, C ABI plugins (`.so`/executables/cffi), C++ only in adapters and third_party. |
| [D9](D9.md) | Provenance on every IR object | Accepted | 2026-10-08 | Pointers in-process plus stable string IDs in serialization and emitted names. |

Format: Nygard style (Context, Decision, Consequences, Related, Revisit if). Phase-0 decisions
in `docs/PHASE0.md` §A.2 are cited from the ADRs they touch and are not ADRs themselves.
