attribute \top 1
module \m
  wire width 8 input 1 signed \a
  wire width 17 input 2 \b
  wire output 3 \y
  cell $eq $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 17
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
