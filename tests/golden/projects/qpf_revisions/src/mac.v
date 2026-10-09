// acc <= acc + a * b.
module mac (
    input  wire       clk,
    input  wire [3:0] a,
    input  wire [3:0] b,
    output reg  [7:0] acc
);
    initial acc = 8'd0;
    always @(posedge clk)
        acc <= acc + a * b;
endmodule

// N-stage delay line (N >= 1).
module delay #(parameter N = 1) (
    input  wire       clk,
    input  wire [7:0] d,
    output wire [7:0] q
);
    reg [7:0] r [0:N-1];
    integer i;

    always @(posedge clk) begin
        r[0] <= d;
        for (i = 1; i < N; i = i + 1)
            r[i] <= r[i-1];
    end
    assign q = r[N-1];
endmodule
