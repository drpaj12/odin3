# generated: elab-check --write
attribute \top 1
module \q2_multi_output
  wire input 1 \a
  wire output 2 \y1
  wire output 3 \y2
  wire output 4 \y3
  wire output 5 \n1
  wire output 6 \n2
  cell $not $c7
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \n1
  end
  cell $not $c8
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \n2
  end
  connect \y1 \a
  connect \y2 \a
  connect \y3 \a
end
