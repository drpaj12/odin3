# generated: elab-check --write
attribute \top 1
module \e18_signed_calls
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 signed \s
  wire width 4 output 4 signed \y1
  wire width 5 output 5 signed \y2
  wire width 4 output 6 \y3
  cell $add $c7
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 5
    parameter \B_WIDTH 5
    parameter \Y_WIDTH 5
    connect \A { \a [3] \a }
    connect \B { \b [3] \b }
    connect \Y \y2
  end
  cell $shr $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 4
    connect \A \s
    connect \B 32'00000000000000000000000000000001
    connect \Y \y3
  end
  connect \y1 \a
end
