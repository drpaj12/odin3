attribute \top 1
module \t
  wire width 3 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 input 5 \d
  wire width 4 output 6 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $pmux $c9
    parameter \WIDTH 4
    parameter \S_WIDTH 3
    connect \A \d
    connect \B { \a \b \c }
    connect \S \s
    connect \Y \y
  end
end
