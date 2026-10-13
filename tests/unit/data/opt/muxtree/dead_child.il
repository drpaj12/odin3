module \top
  wire input 1 \s
  wire input 2 \u
  wire input 3 \a
  wire input 4 \b
  wire input 5 \c
  wire output 6 \y
  wire \t
  cell $mux \in
    parameter \WIDTH 1
    connect \A \b
    connect \B \a
    connect \S \u
    connect \Y \t
  end
  cell $mux \outer
    parameter \WIDTH 1
    connect \A \c
    connect \B \t
    connect \S 1'0
    connect \Y \y
  end
end
