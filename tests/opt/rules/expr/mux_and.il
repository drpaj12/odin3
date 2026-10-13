attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \b
  wire output 3 \y
  cell $and $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B \b
    connect \Y \y
  end
end
