attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire width 4 input 4 \d
  wire width 4 output 5 \y
  wire \en
  cell $mux $c6
    parameter \WIDTH 1
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \en
  end
  attribute \full_case 1
  cell $mux $c10
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \d
    connect \S \en
    connect \Y \y
  end
end
