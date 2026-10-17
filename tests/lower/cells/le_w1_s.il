attribute \top 1
module \m
  wire input 1 signed \a
  wire input 2 signed \b
  wire output 3 \y
  cell $le $c4
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
end
