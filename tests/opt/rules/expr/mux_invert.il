attribute \top 1
module \t
  wire input 1 \s
  wire output 2 \y
  cell $not $c6
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \s
    connect \Y \y
  end
end
