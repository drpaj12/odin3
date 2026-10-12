module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \s
  wire width 2 output 4 \y
  cell $mux \m
    parameter \WIDTH 2
    connect \A { \a \a }
    connect \B { \b \b }
    connect \S \s
    connect \Y \y
  end
end
