attribute \top 1
module \m
  wire width 2 input 1 \a
  wire width 2 input 2 \b
  wire input 3 \s
  wire width 2 output 4 \y
  cell $mux $c5
    parameter \WIDTH 2
    connect \A \a
    connect \B \b
    connect \S \s
    connect \Y \y
  end
end
