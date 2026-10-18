attribute \top 1
module \m
  wire input 1 \a
  wire input 2 \b0
  wire input 3 \b1
  wire width 2 input 4 \s
  wire output 5 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c10
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { \b0 \b1 }
    connect \S { \s [0] \s [1] }
    connect \Y \y
  end
end
