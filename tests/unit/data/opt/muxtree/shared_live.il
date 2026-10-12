module \top
  wire input 1 \s1
  wire input 2 \p
  wire input 3 \q
  wire input 4 \r1
  wire input 5 \r2
  wire output 6 \y1
  wire output 7 \y2
  wire \xw
  cell $mux \x
    parameter \WIDTH 1
    connect \A \q
    connect \B \p
    connect \S \s1
    connect \Y \xw
  end
  cell $mux \m1
    parameter \WIDTH 1
    connect \A \r1
    connect \B \xw
    connect \S \s1
    connect \Y \y1
  end
  cell $mux \m2
    parameter \WIDTH 1
    connect \A \xw
    connect \B \r2
    connect \S \s1
    connect \Y \y2
  end
end
