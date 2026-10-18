attribute \top 1
module \m
  wire input 1 \a
  wire width 4 input 2 \b
  wire output 3 \y
  cell $eq $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
