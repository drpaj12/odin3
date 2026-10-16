attribute \top 1
module \to_unsigned_neg
  wire width 7 input 1 \a
  wire width 8 output 2 signed \y
  cell $neg $c6
    parameter \A_SIGNED 0
    parameter \A_WIDTH 7
    parameter \Y_WIDTH 8
    connect \A \a
    connect \Y \y
  end
end
