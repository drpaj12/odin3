attribute \top 1
module \m
  wire width 3 input 1 \a
  wire width 3 input 2 \b
  wire input 3 \s
  wire width 3 output 4 \y
  cell $mux $c5
    parameter \WIDTH 3
    connect \A \a
    connect \B \b
    connect \S \s
    connect \Y \y
  end
end
