attribute \top 1
module \to_unsigned
  wire width 7 input 1 \a
  wire width 7 input 2 \b
  wire width 8 output 3 signed \y
  cell $add $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 7
    parameter \B_WIDTH 7
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
