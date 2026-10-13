module \top
  wire input 1 \s0
  wire input 2 \s1
  wire input 3 \s2
  wire input 4 \d0
  wire input 5 \d1
  wire input 6 \d2
  wire input 7 \d3
  wire \y0
  wire \y1
  wire \y2
  wire \y3
  wire \y4
  wire \y5
  wire \y6
  wire \y7
  wire \y8
  wire \y9
  wire \y10
  wire \y11
  wire \y12
  wire output 8 \y13
  wire output 9 \y14
  cell $pmux \m0
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A 1'1
    connect \B { 1'0 1'0 1'1 }
    connect \S { \s0 \s1 \s2 }
    connect \Y \y0
  end
  cell $mux \m1
    parameter \WIDTH 1
    connect \A 1'0
    connect \B { 1'1 }
    connect \S { \s2 }
    connect \Y \y1
  end
  cell $pmux \m2
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A \y0
    connect \B { \y1 \y0 }
    connect \S { \s2 \s0 }
    connect \Y \y2
  end
  cell $mux \m3
    parameter \WIDTH 1
    connect \A \y1
    connect \B { \y1 }
    connect \S { \s0 }
    connect \Y \y3
  end
  cell $mux \m4
    parameter \WIDTH 1
    connect \A \y2
    connect \B { \y3 }
    connect \S { \s2 }
    connect \Y \y4
  end
  cell $mux \m5
    parameter \WIDTH 1
    connect \A \y4
    connect \B { \y2 }
    connect \S { \s1 }
    connect \Y \y5
  end
  cell $pmux \m6
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A \d1
    connect \B { \y4 \y4 \y4 }
    connect \S { \s2 \s0 \s1 }
    connect \Y \y6
  end
  cell $mux \m7
    parameter \WIDTH 1
    connect \A \d0
    connect \B { \s2 }
    connect \S { \s0 }
    connect \Y \y7
  end
  cell $mux \m8
    parameter \WIDTH 1
    connect \A \y5
    connect \B { \y5 }
    connect \S { \s2 }
    connect \Y \y8
  end
  cell $mux \m9
    parameter \WIDTH 1
    connect \A \s2
    connect \B { \y0 }
    connect \S { \s1 }
    connect \Y \y9
  end
  cell $pmux \m10
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A \y5
    connect \B { \y0 \y8 \s0 }
    connect \S { \s0 \s1 \s2 }
    connect \Y \y10
  end
  cell $mux \m11
    parameter \WIDTH 1
    connect \A \y8
    connect \B { \y4 }
    connect \S { \s2 }
    connect \Y \y11
  end
  cell $mux \m12
    parameter \WIDTH 1
    connect \A \y10
    connect \B { \s0 }
    connect \S { \s0 }
    connect \Y \y12
  end
  cell $mux \m13
    parameter \WIDTH 1
    connect \A \y12
    connect \B { \y12 }
    connect \S { \s2 }
    connect \Y \y13
  end
  cell $mux \m14
    parameter \WIDTH 1
    connect \A \s1
    connect \B { \y12 }
    connect \S { \s0 }
    connect \Y \y14
  end
end
