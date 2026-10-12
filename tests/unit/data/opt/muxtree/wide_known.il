module \top
  wire input 1 \s
  wire input 2 \u
  wire width 2 input 3 \a
  wire width 2 input 4 \b
  wire width 2 input 5 \c
  wire width 2 output 6 \y
  wire width 2 \t
  cell $mux \in
    parameter \WIDTH 2
    connect \A { \s \u }
    connect \B \b
    connect \S \u
    connect \Y \t
  end
  cell $mux \outer
    parameter \WIDTH 2
    connect \A \c
    connect \B \t
    connect \S \s
    connect \Y \y
  end
end
