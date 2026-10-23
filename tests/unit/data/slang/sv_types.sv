// Fixture with package, struct, enum, interface and always_comb/latch.
package defs;
    typedef enum logic [1:0] {IDLE = 2'd0, RUN = 2'd1, DONE = 2'd2} state_e;
    typedef struct packed { logic [3:0] lo; logic [3:0] hi; } pair_t;
    localparam int W = 8;
endpackage

interface bus_if #(parameter int W = 8) (input logic clk);
    logic [W-1:0] data;
    logic         valid;
    modport slave (input data, valid);
endinterface

module dut (
    input  logic        clk,
    input  logic        rst,
    input  logic [3:0]  a,
    input  logic [3:0]  b,
    output logic [4:0]  sum,
    output logic [3:0]  q
);
    import defs::*;
    state_e state, next_state;
    pair_t  p;
    logic [3:0] lut [0:3];
    logic       g;

    always_comb begin
        sum = a + b;
        p.lo = a;
        p.hi = b;
        g = &sum;
        case (state)
            IDLE:    next_state = RUN;
            RUN:     next_state = DONE;
            default: next_state = IDLE;
        endcase
    end

    always_ff @(posedge clk) begin
        if (rst)
            state <= IDLE;
        else
            state <= next_state;
        q <= p.lo ^ p.hi;
    end

    always_latch begin
        if (a[0])
            lut[0] = b;
    end
endmodule
