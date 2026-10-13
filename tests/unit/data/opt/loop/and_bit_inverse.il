module \m
  wire input 1 \a
  wire output 2 \y
  wire \

  cell $not \inv
    connect \A \a
    connect \Y \

  end
  cell $and \c
    connect \A \a
    connect \B \

    connect \Y \y
  end
end
