// S1 fixture: a two-way traffic light, one clock, two car sensors, two 2-bit lights.
module traffic(input clk, input rst, input car_ns, input car_ew,
               output [1:0] light_ns, output [1:0] light_ew);
    localparam RED = 2'd0, YELLOW = 2'd1, GREEN = 2'd2;
    reg [1:0] state; // 0 NS green, 1 NS yellow, 2 EW green, 3 EW yellow
    reg [2:0] timer;
    always @(posedge clk) begin
        if (rst) begin
            state <= 2'd0;
            timer <= 3'd0;
        end else if (timer != 3'd0) begin
            timer <= timer - 3'd1;
        end else begin
            case (state)
                2'd0: if (car_ew) begin state <= 2'd1; timer <= 3'd1; end
                2'd1: begin state <= 2'd2; timer <= 3'd3; end
                2'd2: if (car_ns) begin state <= 2'd3; timer <= 3'd1; end
                default: begin state <= 2'd0; timer <= 3'd3; end
            endcase
        end
    end
    assign light_ns = state == 2'd0 ? GREEN : state == 2'd1 ? YELLOW : RED;
    assign light_ew = state == 2'd2 ? GREEN : state == 2'd3 ? YELLOW : RED;
endmodule
