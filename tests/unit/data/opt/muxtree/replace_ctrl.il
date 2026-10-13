module \top
  wire input 1 \s0
  wire input 2 \s1
  wire input 3 \a1
  wire input 4 \b0
  wire input 5 \a2
  wire width 2 output 6 \y
  cell $pmux \m
    parameter \WIDTH 2
    parameter \S_WIDTH 2
    connect \A { \s1 \a2 }
    connect \B { \s0 \b0 \a1 \s0 }
    connect \S { \s1 \s0 }
    connect \Y \y
  end
end
