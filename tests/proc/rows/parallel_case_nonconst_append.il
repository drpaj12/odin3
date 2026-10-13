attribute \top 1
module \t
  wire width 2 input 1 \s
  wire width 2 input 2 \k
  wire input 3 \a
  wire input 4 \b
  wire output 5 \y
  wire $n10
  wire $n11
  attribute \full_case 1
  attribute \parallel_case 1
  cell $eq $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B \k
    connect \Y $n10
  end
  attribute \full_case 1
  attribute \parallel_case 1
  cell $mux $c9
    parameter \WIDTH 1
    connect \A 1'x
    connect \B \b
    connect \S $n10
    connect \Y $n11
  end
  connect \y \a
end
