attribute \top 1
module \t
  wire width 4 input 1 \a
  wire output 2 \y
  cell $logic_not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y
  end
end
