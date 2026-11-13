// A testbench that ends itself at time 10.
module t;
  initial begin
    #10;
    $display("done");
    $finish;
  end
endmodule
