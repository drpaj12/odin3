module \top
  wire input 1 \s
  wire input 2 \u
  wire input 3 \p
  wire input 4 \q
  wire input 5 \r
  wire input 6 \c
  wire output 7 \y
  wire \t
  cell $pmux \in
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \r
    connect \B { \q \p }
    connect \S { \u \s }
    connect \Y \t
  end
  cell $mux \outer
    parameter \WIDTH 1
    connect \A \c
    connect \B \t
    connect \S \s
    connect \Y \y
  end
end
