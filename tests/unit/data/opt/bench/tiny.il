module \m
  wire input 1 \a
  wire output 2 \y
  wire \t
  cell $and \c
    connect \A \a
    connect \B 1'0
    connect \Y \y
  end
  cell $_NOT_ \dead
    connect \A \a
    connect \Y \t
  end
end
