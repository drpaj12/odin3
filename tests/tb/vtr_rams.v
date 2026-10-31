// VTR single_port_ram / dual_port_ram behavioural models (from vtr_flow/primitives.v, specify blocks
// removed) for the generated-testbench gate: read by Odin III and by iverilog alike.
module single_port_ram #(
    parameter ADDR_WIDTH = 1,
    parameter DATA_WIDTH = 1
) (
    input clk,
    input [ADDR_WIDTH-1:0] addr,
    input [DATA_WIDTH-1:0] data,
    input we,
    output reg [DATA_WIDTH-1:0] out
);

    localparam MEM_DEPTH = 2 ** ADDR_WIDTH;

    reg [DATA_WIDTH-1:0] Mem[MEM_DEPTH-1:0];



    always@(posedge clk) begin
        if(we) begin
            Mem[addr] <= data;
        end
    	out <= Mem[addr]; //Old data read-first behaviour (non-blocking assignments)
    end

endmodule // single_port_RAM

//dual_port_ram module
(* keep_hierarchy *)
module dual_port_ram #(
    parameter ADDR_WIDTH = 1,
    parameter DATA_WIDTH = 1
) (
    input clk,

    input [ADDR_WIDTH-1:0] addr1,
    input [ADDR_WIDTH-1:0] addr2,
    input [DATA_WIDTH-1:0] data1,
    input [DATA_WIDTH-1:0] data2,
    input we1,
    input we2,
    output reg [DATA_WIDTH-1:0] out1,
    output reg [DATA_WIDTH-1:0] out2
);

    localparam MEM_DEPTH = 2 ** ADDR_WIDTH;

    reg [DATA_WIDTH-1:0] Mem[MEM_DEPTH-1:0];



    always@(posedge clk) begin //Port 1
        if(we1) begin
            Mem[addr1] <= data1;
        end
        out1 <= Mem[addr1]; //Old data read-first behaviour (non-blocking assignments)
    end

    always@(posedge clk) begin //Port 2
        if(we2) begin
            Mem[addr2] <= data2;
        end
        out2 <= Mem[addr2]; //Old data read-first behaviour (non-blocking assignments)
    end

endmodule // dual_port_ram
