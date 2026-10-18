attribute \top 1
module \m
  wire width 2 input 1 \a
  wire width 2 input 2 \b0
  wire width 2 input 3 \b1
  wire width 2 input 4 \s
  wire width 2 output 5 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c10
    parameter \WIDTH 2
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { \b0 \b1 }
    connect \S { \s [0] \s [1] }
    connect \Y \y
  end
end
