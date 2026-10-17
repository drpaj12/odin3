attribute \top 1
module \divmod_trim_mod_signed
  wire width 5 input 1 signed \a
  wire width 5 input 2 signed \b
  wire width 8 output 3 signed \y
  cell $mod $c6
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 6
    parameter \B_WIDTH 7
    parameter \Y_WIDTH 8
    connect \A { 1'0 \a }
    connect \B { 1'0 \b 1'0 }
    connect \Y \y
  end
end
