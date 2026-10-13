module \m
  wire input 1 \a
  wire input 2 \c
  wire input 3 \d
  wire output 4 \y
  cell $eq \q
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    connect \A { \a \c }
    connect \B { \a \d }
    connect \Y \y
  end
end
