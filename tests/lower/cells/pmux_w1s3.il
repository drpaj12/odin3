attribute \top 1
module \m
  wire input 1 \a
  wire input 2 \b0
  wire input 3 \b1
  wire input 4 \b2
  wire width 3 input 5 \s
  wire output 6 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c12
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A \a
    connect \B { \b0 \b1 \b2 }
    connect \S { \s [0] \s [1] \s [2] }
    connect \Y \y
  end
end
