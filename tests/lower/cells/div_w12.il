attribute \top 1
module \m
  wire width 12 input 1 \a
  wire width 12 input 2 \b
  wire width 12 output 3 \y
  cell $div $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 12
    parameter \B_WIDTH 12
    parameter \Y_WIDTH 12
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
