attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \t
  wire width 2 input 3 \a
  wire width 2 input 4 \c
  wire width 2 input 5 \d
  wire width 2 output 6 \y
  attribute \full_case 1
  attribute \parallel_case 1
  cell $mux $c15
    parameter \WIDTH 2
    connect \A \a
    connect \B \d
    connect \S \t
    connect \Y \y
  end
end
