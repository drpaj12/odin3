attribute \top 1
module \m
  wire input 1 \a
  wire input 2 \b0
  wire input 3 \b1
  wire input 4 \b2
  wire input 5 \b3
  wire width 4 input 6 \s
  wire output 7 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c14
    parameter \WIDTH 1
    parameter \S_WIDTH 4
    connect \A \a
    connect \B { \b0 \b1 \b2 \b3 }
    connect \S { \s [0] \s [1] \s [2] \s [3] }
    connect \Y \y
  end
end
