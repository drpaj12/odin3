attribute \top 1
module \m
  wire width 2 input 1 \a
  wire width 2 input 2 \b0
  wire width 2 input 3 \b1
  wire width 2 input 4 \b2
  wire width 2 input 5 \b3
  wire width 4 input 6 \s
  wire width 2 output 7 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c14
    parameter \WIDTH 2
    parameter \S_WIDTH 4
    connect \A \a
    connect \B { \b0 \b1 \b2 \b3 }
    connect \S { \s [0] \s [1] \s [2] \s [3] }
    connect \Y \y
  end
end
