// S3: system tasks as statements (IGNORE) — variants: $display with a string and arguments, $display with an empty argument, $write, $monitor, $finish, $stop with an argument
module s3_system_tasks (input clk, a, b);
  always @(posedge clk) begin
    $display("a=%b b=%b", a, b);
    $display(a, , b);
    $write("w");
    if (a)
      $finish;
    if (b)
      $stop(1);
  end
  initial $monitor("%b", a);
endmodule
