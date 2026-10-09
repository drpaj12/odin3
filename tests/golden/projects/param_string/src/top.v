// A string parameter picks the operation; the project sets MODE to "sub".
module top #(parameter MODE = "add") (
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [3:0] y
);
    generate
        if (MODE == "sub") begin : g_sub
            assign y = a - b;
        end else begin : g_add
            assign y = a + b;
        end
    endgenerate
endmodule
