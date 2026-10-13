attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \y
  attribute \full_case 1
  cell $mux $c7
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \y
  end
end
