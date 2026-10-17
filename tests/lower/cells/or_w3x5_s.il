attribute \top 1
module \m
  wire width 3 input 1 signed \a
  wire width 5 input 2 signed \b
  wire width 5 output 3 \y
  cell $or $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 3
    parameter \B_WIDTH 5
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
