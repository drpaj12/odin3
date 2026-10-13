module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire input 4 \s0
  wire input 5 \s1
  wire input 6 \s2
  wire output 7 \y
  cell $pmux \p
    parameter \WIDTH 1
    parameter \S_WIDTH 3
    connect \A \a
    connect \B { \b \c \b }
    connect \S { \s2 \s1 \s0 }
    connect \Y \y
  end
end
