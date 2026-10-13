module \m
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire input 4 \s0
  wire input 5 \s1
  wire output 6 \y
  wire output 7 \z
  cell $pmux \c1
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { \c \b }
    connect \S { \s1 \s0 }
    connect \Y \y
  end
  cell $pmux \c2
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { \b \c }
    connect \S { \s0 \s1 }
    connect \Y \z
  end
end
