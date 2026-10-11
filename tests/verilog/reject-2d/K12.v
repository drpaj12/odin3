// K12: a timing control inside a function (REJECT).
module m (input a, output y);
  function f;
    input i;
    begin
      #1 f = i;
    end
  endfunction
  assign y = f(a);
endmodule
