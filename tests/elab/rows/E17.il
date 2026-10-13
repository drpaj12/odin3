# generated: elab-check --write
attribute \top 1
module \e17_out_of_range
  wire width 8 input 1 \a
  wire width 4 input 2 \s
  wire output 3 \y1
  wire output 4 \y2
  wire output 5 \y3
  wire output 6 \y4
  cell $shiftx $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \s
    connect \Y \y4
  end
  connect \y1 1'x
  connect \y2 1'x
  connect \y3 1'x
end
