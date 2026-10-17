attribute \top 1
module \m
  wire width 3 input 1 signed \a
  wire width 5 input 2 \b
  wire output 3 \y
  cell $gt $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 5
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
