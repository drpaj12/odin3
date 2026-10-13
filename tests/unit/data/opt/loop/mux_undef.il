module \m
  wire input 1 \a
  wire input 2 \b
  wire input 3 \s0
  wire input 4 \s1
  wire output 5 \y
  cell $pmux \c
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { 1'x \b }
    connect \S { \s0 \s1 }
    connect \Y \y
  end
end
