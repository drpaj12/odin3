# generated: elab-check --write
attribute \top 1
module \g5_if_gen
  wire width 4 input 1 \a
  wire width 4 output 2 \y1
  wire width 4 output 3 \y2
  wire width 4 output 4 \y3
  cell $not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \Y \y1
  end
  connect \y2 \a
  connect \y3 \a
end
