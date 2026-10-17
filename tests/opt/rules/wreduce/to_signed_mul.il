attribute \top 1
module \to_signed_mul
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 8 output 3 \y
  cell $mul $c5
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
