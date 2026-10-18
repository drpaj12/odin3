attribute \top 1
module \m
  wire width 6 input 1 signed \a
  wire width 2 input 2 signed \b
  wire width 6 output 3 \y
  cell $div $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 6
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 6
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
