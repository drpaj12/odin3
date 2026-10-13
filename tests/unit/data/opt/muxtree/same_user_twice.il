module \top
  wire input 1 \s
  wire input 2 \u
  wire input 3 \p
  wire input 4 \q
  wire output 5 \y
  wire \t
  cell $mux \in
    parameter \WIDTH 1
    connect \A \q
    connect \B \p
    connect \S \u
    connect \Y \t
  end
  cell $mux \outer
    parameter \WIDTH 1
    connect \A \t
    connect \B \t
    connect \S \s
    connect \Y \y
  end
end
