module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire input 4 \d
  wire input 5 \s
  wire width 2 output 6 \y
  cell $pmux \p
    parameter \WIDTH 2
    parameter \S_WIDTH 3
    connect \A { \a \a }
    connect \B { \b \b \c \c \d \d }
    connect \S { \s 1'1 1'0 }
    connect \Y \y
  end
end
